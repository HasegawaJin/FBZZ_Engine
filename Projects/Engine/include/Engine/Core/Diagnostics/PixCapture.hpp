/// @file    PixCapture.hpp
/// @brief   明示指定で有効にする PIX 起動設定と初期ロードの検証結果。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Engine/Api.hpp>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace fbzz::core {

inline constexpr std::wstring_view PIX_CAPTURE_STABLE_VERSION = L"2603.25";

struct PixCaptureOptions {
    bool requested = false;
    std::filesystem::path installedDirectory; ///< 空なら Program Files 配下の固定した安定版を選ぶ。明示パスは絶対ディレクトリでなければならない。
};

struct PixCaptureStatus {
    bool requested = false;
    bool ready = false; ///< 起動ローダーが選択したモジュールを検証した場合のみ true。ディスク上の存在確認だけでは true にならない。
    std::string reason;
    std::filesystem::path installedDirectory; ///< 解決したキャプチャ DLL のディレクトリ。要求に失敗しても値が残る場合があり、準備完了の判定には使わない。
};

/// @brief 分割済みの PIX 引数を解析する。ファイルシステムへのアクセスやモジュールのロードは行わない。
/// @note 他のオプションの引数は呼び出し側が先に消費する。PIX と無関係の引数は無視する。
/// @return false の場合は out を変更せず理由を返す。成功時は reason を空にする。
[[nodiscard]] FBZZ_ENGINE_API bool ParsePixCaptureOptions(std::span<const std::wstring_view> arguments,
                                         PixCaptureOptions& out, std::string& reason);

/// @brief 明示指定のディレクトリを字句的に解決する。ディスク確認、プロセス状態の変更、モジュールのロードは行わない。
/// @note オプションが指定されていなければ、Program Files を取得できなくても空のパスを返す。
/// @return false の場合は out を変更せず理由を返す。成功時は reason を空にする。
[[nodiscard]] FBZZ_ENGINE_API bool ResolvePixCaptureDirectory(const PixCaptureOptions& options,
                                             const std::filesystem::path& programFilesDirectory,
                                             std::filesystem::path& out, std::string& reason);

/// @brief 選択したキャプチャモジュールだけをロードし、プロセス終了まで参照を保持する。
/// @pre D3D12 API の初回呼び出し前に起動スレッドから呼ぶ。ランチャーは事前に対応する DX12 バックエンドを選ぶ。
/// @note 起動オプションは初回呼び出し後に変更できない。通常起動では PIX をロードせず、失敗時は再試行しない。
/// @return false の場合は失敗をログに記録し、準備完了を報告しない。呼び出し側はレンダラー作成前に起動を中止する。
/// @see https://devblogs.microsoft.com/pix/taking-a-capture/ D3D12 の初期化より先に対応する WinPixGpuCapturer.dll をロードする条件。
[[nodiscard]] FBZZ_ENGINE_API bool InitializePixCapture(const PixCaptureOptions& options);

/// @brief プロセス起動時の検証結果を読む。探索、ロード、注入、再起動は行わない。
/// @note 起動処理での初期化後にメインスレッドから読む。参照先の状態はプロセス終了まで有効。
[[nodiscard]] FBZZ_ENGINE_API const PixCaptureStatus& GetPixCaptureStatus();

} // namespace fbzz::core
