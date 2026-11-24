/// @file    NamedPipeServer.hpp
/// @brief   Editor Command Bus の受け口。Windows Named Pipe を待ち受け、NDJSON 要求をメインスレッドへ渡す。
/// @author  Hasegawa Jin
/// @date    2026-07-20
/// @note MCP サーバ (Node) は接続毎に1要求→1応答→切断する NDJSON クライアント。並行クライアントに備え
/// @note PIPE_UNLIMITED_INSTANCES で複数接続を受ける。
/// @note IO はワーカースレッドで行い、シーン変更を伴う実処理は必ずメインスレッド (DrainRequests) で実行する。
/// @note Win32 の詳細 (HANDLE / OVERLAPPED) は pimpl で隠蔽し、上位に <windows.h> を波及させない。
#pragma once
#include <functional>
#include <memory>
#include <string>

namespace fbzz::editor::ai {

class NamedPipeServer {
public:
    /// @note 受信した1行 (NDJSON 要求) を受け取り、応答1行 (改行なし) を返すハンドラ。DrainRequests 内で呼ばれる。
    using RequestHandler = std::function<std::string(const std::string& requestLine)>;

    NamedPipeServer();
    ~NamedPipeServer();
    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    /// @note pipeName 例: L"\\\\.\\pipe\\FBZZEditorCommandBus"。listener スレッドを起動する。失敗時 false。
    /// @return 最初の待受パイプを作成できた場合に true。失敗時は起動途中の資源を解放する。
    bool Start(const std::wstring& pipeName);

    /// @note 所有スレッドから停止する。I/O 完了と全 worker の join 後に戻る。繰り返し呼出し・handler 内からの停止も可。
    void Stop();

    bool IsRunning() const;

    /// @note メインスレッドで毎フレーム呼ぶ。溜まった要求を handler で処理し、待機中ワーカーへ応答を返す。
    void DrainRequests(const RequestHandler& handler);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} /// @note namespace fbzz::editor::ai
