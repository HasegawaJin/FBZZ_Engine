/// @file    AssetFileWatcher.hpp
/// @brief   Assets/ ディレクトリの変化をポーリングで検出するファイルシステム監視。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// @note ReadDirectoryChangesW の非同期オーバーラップモードで std::thread なしに変化通知を取得する (シングルスレッド方針に準拠)。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

class AssetFileWatcher {
public:
    enum class EventType { Added, Modified, Removed, Renamed };

    struct FileEvent {
        EventType   type;
        std::string path;     ///< rootPath からの相対パス ('/' 区切り)
        std::string oldPath;  ///< Renamed 時のみ有効
    };

    AssetFileWatcher();
    ~AssetFileWatcher();

    AssetFileWatcher(const AssetFileWatcher&)            = delete;
    AssetFileWatcher& operator=(const AssetFileWatcher&) = delete;

    /// rootPath 以下を再帰的に監視する。既に起動中なら Stop してから再起動する。
    /// @return 成功なら true
    bool Start(const std::string& rootPath);

    void Stop();

    /// 溜まったイベントを返して内部キューをクリアする。毎フレーム呼ぶ。
    /// Start() していない場合は空を返す。
    std::vector<FileEvent> Poll();

    [[nodiscard]] bool        IsRunning()  const;
    [[nodiscard]] std::string GetRootPath() const { return m_rootPath; }

    /// 通知バッファが溢れてイベントを取りこぼしたか。true なら 1 回だけ返して印を消す。
    /// @note オーバーフロー時は個々のイベントで追従できないため、呼び出し側は一覧とキャッシュを丸ごと作り直す。
    [[nodiscard]] bool ConsumeOverflow() { const bool o = m_overflowed; m_overflowed = false; return o; }

private:
    struct NativeWatch;

    /// 次の ReadDirectoryChangesW を発行する
    void IssueNextRead();

    /// 通知バッファを解析してイベントキューに積む
    void ParseBuffer(std::uint32_t bytesTransferred);

    std::unique_ptr<NativeWatch> m_native; ///< 監視に使う OS 側のハンドルと通知バッファの所有者。
    std::string m_rootPath; ///< 監視するファイルツリーのルートパス。

    std::vector<FileEvent> m_queue; ///< Editor の次回ポーリングへ渡すために蓄積したファイル通知。
    /// FILE_ACTION_RENAMED_OLD_NAME と NEW_NAME が別の通知バッファへ分割されても
    /// ペアを失わないため、ParseBuffer をまたいで一時保持する。
    std::string m_pendingRenameOld; ///< 名前変更の新しいパスの通知を待っている間に控える古いパス。
    bool m_readPending = false; ///< OS からの非同期読み取りの完了を待っているか。
    bool m_overflowed  = false; ///< 通知を取りこぼしたためルート以下の再走査が必要か。
};

} // namespace fbzz::editor
