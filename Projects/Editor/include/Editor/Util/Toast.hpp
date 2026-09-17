/// @file    Toast.hpp
/// @brief   画面右下に数秒表示して自動的に消える非モーダル通知 (トースト)。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// @note ビルド失敗バーが扱うのは「消えずに残すべき失敗」のみで、短命な成功通知はここに集約する。
///       静的 API なのは発火元 (シーン I/O・AssetBrowser・各パネル) が状態を持ち回らずに済むため。
/// @note 呼び出しはすべてメインスレッド (ImGui フレーム内) を前提とし、ロックは持たない。
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

class Toast {
public:
    enum class Level { Info, Success, Warning, Error };

    /// 任意レベルの通知を積む。durationSec はフェードアウトを含む総表示時間 (秒)。
    static void Push(Level level, std::string message, float durationSec = 3.5f);

    /// レベル別のショートハンド。
    static void Info(std::string message)    { Push(Level::Info,    std::move(message)); }
    static void Success(std::string message) { Push(Level::Success, std::move(message)); }
    static void Warning(std::string message) { Push(Level::Warning, std::move(message), 5.0f); }
    static void Error(std::string message)   { Push(Level::Error,   std::move(message), 6.0f); }

    /// EditorApp::RenderPanels() 末尾から毎フレーム呼ぶ。
    /// @note すべての通常ウィンドウの後に描画することで、トーストを最前面へ重ねる。
    static void Render();

private:
    struct Entry {
        Level       level;
        std::string message;
        /// @note 経過秒
        float       age      = 0.0f;
        /// @note 総表示秒 (フェードアウト含む)
        float       duration = 3.5f;
    };
    static std::vector<Entry> s_entries;
};

} // namespace fbzz::editor
